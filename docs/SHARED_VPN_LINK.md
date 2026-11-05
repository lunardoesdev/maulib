# SharedVPNLink API

`SharedVPNLink` — объект, созданный из share-ссылки (`vless://`, `vmess://`,
`trojan://`, `ss://`, ...). Он объединяет:

1. прокси в группе `MILIB` (участвует в hot-swap через `milib_select`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` с обязательной
   авторизацией;
3. методы `ping`, `speed`, `exit_ip` и их async-варианты.

Заголовок: `include/milib.h`. Примеры: `tests/python/test_links.py`,
`tests/e2e/run_links.py`.

## Создание

```c
char* err = NULL;
milib_link* link = milib_link_create(h, "vless://...", options_json, &err);
if (link == NULL) { /* err */ }
```

`options_json`:

```json
{
  "name": "my-node",
  "select": false,
  "local_proxy": true,
  "username": "milib",
  "password": "manual-pass",
  "proxy_password": "upstream-secret"
}
```

| Поле | По умолчанию | Смысл |
|---|---|---|
| `name` | fragment / type-server-port | имя прокси и ссылки |
| `select` | `false` | сразу выбрать прокси в `MILIB` |
| `local_proxy` | `true` | поднять per-link SOCKS5 |
| `username` | `milib` | логин локального SOCKS5 |
| `password` | случайные 18 байт | пароль локального SOCKS5 |
| `proxy_password` | из ссылки | переопределить upstream credential |

Если `password` не задан, генерируется криптостойкий ключ. Это и есть
«авторизация с генерируемыми ключами»; пароль можно задать вручную.

`proxy_password` подставляет credential в зависимости от протокола:

- VLESS/VMess → `uuid`;
- Trojan/Shadowsocks/Hysteria/... → `password`;
- SOCKS5/HTTP → `username`/`password`.

## Информация

```c
char* info = milib_link_info(link);
```

```json
{
  "ok": true,
  "link_id": 1,
  "name": "my-node",
  "type": "VLESS",
  "server": "example.com",
  "port": 443,
  "link": "vless://...",
  "selected": false,
  "socks": {
    "addr": "127.0.0.1:45678",
    "username": "milib",
    "password": "a1b2..."
  },
  "auth": {"username": "milib", "password": "a1b2..."}
}
```

`milib_link_list(h)` возвращает `{"ok":true,"count":N,"links":[...]}`.

## Ping

```c
char* result = milib_link_ping(link, "{\"url\":\"https://www.gstatic.com/generate_204\"}");
```

Задержка измеряется **трафиком через прокси** (`URLTest`: HTTP HEAD через
VLESS/...), а не ICMP-пингом до сервера прокси.

```json
{"ok":true,"link_id":1,"name":"my-node","delay":123,"url":"...","mode":"proxy"}
```

Опции: `url`, `timeout_ms`, `expected` (например `"204"` или `"200-299"`).

## Speed

```c
char* result = milib_link_speed(link,
    "{\"url\":\"https://speed.cloudflare.com/__down?bytes=5242880\",\"bytes\":5242880}");
```

```json
{
  "ok": true,
  "link_id": 1,
  "bytes": 5242880,
  "seconds": 1.23,
  "bytes_per_second": 4263000,
  "mbps": 34.1,
  "status": 200,
  "url": "..."
}
```

Опции: `url`, `bytes` (по умолчанию 5 MiB, максимум 64 MiB), `timeout_ms`.

## Exit IP

```c
char* result = milib_link_exit_ip(link, NULL);
```

```json
{"ok":true,"link_id":1,"ip":"203.0.113.7","valid":true,"status":200,"url":"https://api.ipify.org"}
```

Опции: `url` (по умолчанию `https://api.ipify.org`), `format`
(`text`/`json`), `timeout_ms`. Для JSON поддерживаются поля `ip`, `query`,
`origin`.

## Много прокси одновременно

```c
char* result = milib_links_ping_many(h, "[1,2,3]",
    "{\"url\":\"https://www.gstatic.com/generate_204\",\"timeout_ms\":5000}");
```

Все пинги выполняются в отдельных горутинах; результат сохраняет порядок ids:

```json
{
  "ok": true,
  "count": 3,
  "results": [
    {"ok":true,"link_id":1,"delay":42},
    {"ok":true,"link_id":2,"delay":57},
    {"ok":false,"link_id":3,"error":"delay test failed: ..."}
  ]
}
```

Async-варианты (`milib_link_ping_async`, `milib_link_speed_async`,
`milib_link_exit_ip_async`, `milib_links_ping_many_async`) возвращают task id;
результат приходит в `milib_task_cb` или забирается `milib_task_poll`.

## Маршрутизация трафика

Через глобальный mixed inbound и выбранную ссылку:

```bash
milib_select(h, NULL, "my-node");
curl -x http://127.0.0.1:<mixed_port> https://example.com
```

Через конкретную ссылку, независимо от выбора:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user milib:<password> https://example.com
```

Per-link SOCKS5 поддерживает TCP. UDP не проксируется.

## Удаление

```c
milib_link_destroy(link);
```

Удаляет прокси из `MILIB`, закрывает SOCKS-listener, рвёт соединения с цепочкой
этого прокси и освобождает C-handle. После вызова handle использовать нельзя.
Прокси, принадлежащий ссылке, нельзя удалить через `milib_remove_proxy` —
вернётся ошибка с подсказкой использовать `milib_link_destroy`.

## Потоки и безопасность

- все функции thread-safe;
- несколько ping/speed/exit_ip задач могут идти одновременно;
- `milib_select` во время активного соединения не рвёт его: старые соединения
  продолжают идти через прежний прокси, новые — через новый;
- callback нельзя использовать для вызова `milib_shutdown`.
