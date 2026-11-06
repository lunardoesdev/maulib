# SharedVPNLink API (v2)

`SharedVPNLink` — объект, созданный из share-ссылки (`vless://`, `vmess://`,
`trojan://`, `ss://`, ...). Он объединяет:

1. прокси в группе `MEWNET` (участвует в hot-swap через `mewnet_select_proxy`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` с обязательной
   авторизацией;
3. методы `ping`, `speed`, `exit_ip` и их async-варианты.

Заголовки: `libs/mewnet/cmd/mewnet/mewnet.h`, `libs/mewnet/cmd/mewnet/mewnet_types.h`.
ABI v2 — структуры C, никаких JSON-строк. Ошибки возвращаются в результате,
для простых операций — кодом `mewnet_error_code`.

## Создание

```c
mewnet_link_options options = mewnet_link_options_default();
options.name = "my-node";
options.select_proxy = true;
options.username = "mewnet";
options.password = "manual-pass";
options.proxy_password = "upstream-secret";

mewnet_link_result created = mewnet_link_create(instance, "vless://...", &options);
if (created.link == NULL) {
    fprintf(stderr, "%s\n", created.error.message);
}
mewnet_link *link = created.link;
```

`mewnet_link_options_default()` ставит `local_proxy = true` и
`select_proxy = false`. Не используйте полностью нулевую структуру, если нужен
локальный SOCKS5: берите default и меняйте поля.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `name` | fragment / type-server-port | имя прокси и ссылки |
| `select_proxy` | `false` | сразу выбрать прокси в `MEWNET` |
| `local_proxy` | `true` | поднять per-link SOCKS5 |
| `username` | `mewnet` | логин локального SOCKS5 |
| `password` | случайные 18 байт | пароль локального SOCKS5 |
| `proxy_password` | из ссылки | переопределить upstream credential |

`proxy_password` подставляет credential в зависимости от протокола:

- VLESS/VMess → `uuid`;
- Trojan/Shadowsocks/Hysteria/... → `password`;
- SOCKS5/HTTP → `username`/`password`.

## Информация

```c
mewnet_link_info info = mewnet_link_get_info(link);
if (info.ok) {
    printf("id=%llu name=%s type=%s server=%s port=%d\n",
           (unsigned long long)info.id, info.name, info.proxy_type,
           info.server, info.port);
    printf("socks5 %s user=%s pass=%s selected=%d\n",
           info.socks_addr, info.socks_username, info.socks_password,
           info.selected);
}
```

Поля: `ok`, `error`, `id`, `name`, `proxy_type`, `server`, `port`, `uri`,
`socks_addr`, `socks_username`, `socks_password`, `selected`.

Список всех ссылок:

```c
mewnet_link_list links = mewnet_load_links(instance);
for (size_t i = 0; i < links.count; i++) {
    printf("%s\n", links.items[i].name);
}
mewnet_unload_links(links);
```

## Ping

```c
mewnet_delay_options delay = mewnet_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;
delay.expected = "204";

mewnet_ping_result ping = mewnet_link_ping(link, &delay);
if (ping.ok) {
    printf("%u\n", ping.delay_ms);
} else {
    fprintf(stderr, "ping error code: %d\n", ping.error);
}
```

Задержка измеряется **трафиком через прокси** (`URLTest`: HTTP HEAD через
VLESS/...), а не ICMP-пингом до сервера прокси.

## Speed

```c
mewnet_speed_options speed_options = mewnet_speed_options_default();
speed_options.url = "https://speed.cloudflare.com/__down?bytes=5242880";
speed_options.bytes = 5242880;

mewnet_speed_result speed = mewnet_link_speed(link, &speed_options);
if (speed.ok) {
    printf("%lld bytes in %.2f s = %.2f Mbit/s\n",
           speed.bytes, speed.seconds, speed.mbps);
}
```

Поля результата: `ok`, `error`, `status`, `bytes`, `seconds`,
`bytes_per_second`, `mbps`.

## Exit IP

```c
mewnet_exit_ip_options exit_options = mewnet_exit_ip_options_default();
/* exit_options.url = "https://api.ipify.org"; */
/* exit_options.format = MEWNET_EXIT_IP_JSON; */

mewnet_exit_ip_result exit_ip = mewnet_link_exit_ip(link, &exit_options);
if (exit_ip.ok && exit_ip.valid) {
    printf("%s\n", exit_ip.ip);
}
```

Опции: `url` (по умолчанию `https://api.ipify.org`), `format`
(`MEWNET_EXIT_IP_TEXT` / `MEWNET_EXIT_IP_JSON`), `timeout_ms`.

## Много прокси одновременно

```c
mewnet_link *links[] = { link1, link2, link3 };

mewnet_delay_options delay = mewnet_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;

mewnet_ping_many_result many =
    mewnet_link_ping_many(instance, links, 3, &delay);
for (size_t i = 0; i < many.count; i++) {
    if (many.items[i].ok) {
        printf("%zu: %u ms\n", i, many.items[i].delay_ms);
    } else {
        printf("%zu: error %d\n", i, many.items[i].error);
    }
}
mewnet_unload_ping_many(many);
```

Все пинги выполняются в отдельных горутинах; результат сохраняет порядок
входного массива ссылок.

## Async

Callback получает `mewnet_task_result`:

```c
static void on_done(const mewnet_task_result *result, void *user) {
    if (result->state == MEWNET_TASK_DONE && result->error == MEWNET_ERROR_NONE) {
        printf("delay: %u\n", result->ping.delay_ms);
    }
}

mewnet_task *task = mewnet_link_ping_async(link, &delay, on_done, NULL);
/* ... */
mewnet_task_destroy(task);
```

Либо poll без callback:

```c
mewnet_task *task = mewnet_link_ping_async(link, &delay, NULL, NULL);
mewnet_task_result result;

while ((result = mewnet_task_poll(task)).state == MEWNET_TASK_RUNNING) {
    /* main loop */
}
mewnet_task_destroy(task);
```

Правила:

- callback вызывается на произвольном OS-потоке;
- в callback нельзя вызывать `mewnet_close`, `mewnet_link_destroy`,
  `mewnet_task_destroy`;
- `mewnet_link_destroy` отменяет и ждёт задачи этой ссылки;
- `mewnet_close` отменяет и ждёт все задачи инстанса;
- `mewnet_task_cancel` только запрашивает отмену;
- `mewnet_task_destroy` отменяет, ждёт и освобождает задачу.

Поля `mewnet_task_result`: `state`, `kind`, `error`, `delay`, `ping`, `speed`,
`exit_ip`, `pings`, `ping_count`.

## Маршрутизация трафика

Через глобальный mixed inbound и выбранную ссылку:

```bash
mewnet_select_proxy(instance, "my-node");
curl -x http://127.0.0.1:<mixed_port> https://example.com
```

Через конкретную ссылку, независимо от выбора:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<password> https://example.com
```

Per-link SOCKS5 поддерживает TCP. UDP не проксируется.

## Как проверить ping из C

Готовый пример — `tests/c/test_ping.c`:

```bash
cc tests/c/test_ping.c -Iinclude -Ldist -lmewnet \
   -Wl,-rpath,'$ORIGIN' -o dist/test_ping

./dist/test_ping 'vless://uuid@host:443?...#node' \
                 'https://www.gstatic.com/generate_204'
```

Или через переменные окружения:

```bash
MEWNET_TEST_VLESS='vless://...' ./dist/test_ping
```

Проверка без реальной ссылки (поднимает локальный Xray и target `204`):

```bash
make test-c-ping-local
```

Пример вывода:

```text
3
```

Это задержка в миллисекундах.

## Удаление

```c
mewnet_link_destroy(link);
```

Удаляет прокси из `MEWNET`, закрывает SOCKS-listener, отменяет и ждёт async
задачи этой ссылки, рвёт соединения с цепочкой прокси и освобождает handle.
После вызова handle использовать нельзя. Прокси, принадлежащий ссылке, нельзя
удалить через `mewnet_remove_proxy` — вернётся ошибка, а в лог будет подсказка
использовать `mewnet_link_destroy`.

## Потоки и безопасность

- операции с `mewnet_instance*` и `mewnet_link*` можно вызывать из разных потоков;
- `mewnet_close` и `mewnet_link_destroy` должны быть эксклюзивными: не
  вызывайте их одновременно с другими операциями над тем же объектом;
- callback приходит на произвольном потоке; для UI используйте poll или
  перекладывайте результат в свою очередь;
- log callback тоже вызывается на произвольном потоке;
- не вызывайте `mewnet_close`/destroy из callback;
- panic в фоновой горутине mihomo не перехватывается — это ограничение
  in-process библиотеки.
