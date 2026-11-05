# SharedVPNLink API (v2)

`SharedVPNLink` — объект, созданный из share-ссылки (`vless://`, `vmess://`,
`trojan://`, `ss://`, ...). Он объединяет:

1. прокси в группе `MILIB` (участвует в hot-swap через `milib_select_proxy`);
2. локальный SOCKS5 endpoint на `127.0.0.1:<random>` с обязательной
   авторизацией;
3. методы `ping`, `speed`, `exit_ip` и их async-варианты.

Заголовки: `include/milib.h`, `include/milib_types.h`.
ABI v2 — структуры C, никаких JSON-строк. Ошибки возвращаются в результате,
для простых операций — кодом `milib_error_code`.

## Создание

```c
milib_link_options options = milib_link_options_default();
options.name = "my-node";
options.select_proxy = true;
options.username = "milib";
options.password = "manual-pass";
options.proxy_password = "upstream-secret";

milib_link_result created = milib_link_create(instance, "vless://...", &options);
if (created.link == NULL) {
    fprintf(stderr, "%s\n", created.error.message);
}
milib_link *link = created.link;
```

`milib_link_options_default()` ставит `local_proxy = true` и
`select_proxy = false`. Не используйте полностью нулевую структуру, если нужен
локальный SOCKS5: берите default и меняйте поля.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `name` | fragment / type-server-port | имя прокси и ссылки |
| `select_proxy` | `false` | сразу выбрать прокси в `MILIB` |
| `local_proxy` | `true` | поднять per-link SOCKS5 |
| `username` | `milib` | логин локального SOCKS5 |
| `password` | случайные 18 байт | пароль локального SOCKS5 |
| `proxy_password` | из ссылки | переопределить upstream credential |

`proxy_password` подставляет credential в зависимости от протокола:

- VLESS/VMess → `uuid`;
- Trojan/Shadowsocks/Hysteria/... → `password`;
- SOCKS5/HTTP → `username`/`password`.

## Информация

```c
milib_link_info info = milib_link_get_info(link);
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
milib_link_list links = milib_load_links(instance);
for (size_t i = 0; i < links.count; i++) {
    printf("%s\n", links.items[i].name);
}
milib_unload_links(links);
```

## Ping

```c
milib_delay_options delay = milib_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;
delay.expected = "204";

milib_ping_result ping = milib_link_ping(link, &delay);
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
milib_speed_options speed_options = milib_speed_options_default();
speed_options.url = "https://speed.cloudflare.com/__down?bytes=5242880";
speed_options.bytes = 5242880;

milib_speed_result speed = milib_link_speed(link, &speed_options);
if (speed.ok) {
    printf("%lld bytes in %.2f s = %.2f Mbit/s\n",
           speed.bytes, speed.seconds, speed.mbps);
}
```

Поля результата: `ok`, `error`, `status`, `bytes`, `seconds`,
`bytes_per_second`, `mbps`.

## Exit IP

```c
milib_exit_ip_options exit_options = milib_exit_ip_options_default();
/* exit_options.url = "https://api.ipify.org"; */
/* exit_options.format = MILIB_EXIT_IP_JSON; */

milib_exit_ip_result exit_ip = milib_link_exit_ip(link, &exit_options);
if (exit_ip.ok && exit_ip.valid) {
    printf("%s\n", exit_ip.ip);
}
```

Опции: `url` (по умолчанию `https://api.ipify.org`), `format`
(`MILIB_EXIT_IP_TEXT` / `MILIB_EXIT_IP_JSON`), `timeout_ms`.

## Много прокси одновременно

```c
milib_link *links[] = { link1, link2, link3 };

milib_delay_options delay = milib_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.timeout_ms = 5000;

milib_ping_many_result many =
    milib_link_ping_many(instance, links, 3, &delay);
for (size_t i = 0; i < many.count; i++) {
    if (many.items[i].ok) {
        printf("%zu: %u ms\n", i, many.items[i].delay_ms);
    } else {
        printf("%zu: error %d\n", i, many.items[i].error);
    }
}
milib_unload_ping_many(many);
```

Все пинги выполняются в отдельных горутинах; результат сохраняет порядок
входного массива ссылок.

## Async

Callback получает `milib_task_result`:

```c
static void on_done(const milib_task_result *result, void *user) {
    if (result->state == MILIB_TASK_DONE && result->error == MILIB_ERROR_NONE) {
        printf("delay: %u\n", result->ping.delay_ms);
    }
}

milib_task *task = milib_link_ping_async(link, &delay, on_done, NULL);
/* ... */
milib_task_destroy(task);
```

Либо poll без callback:

```c
milib_task *task = milib_link_ping_async(link, &delay, NULL, NULL);
milib_task_result result;

while ((result = milib_task_poll(task)).state == MILIB_TASK_RUNNING) {
    /* main loop */
}
milib_task_destroy(task);
```

Правила:

- callback вызывается на произвольном OS-потоке;
- в callback нельзя вызывать `milib_close`, `milib_link_destroy`,
  `milib_task_destroy`;
- `milib_link_destroy` отменяет и ждёт задачи этой ссылки;
- `milib_close` отменяет и ждёт все задачи инстанса;
- `milib_task_cancel` только запрашивает отмену;
- `milib_task_destroy` отменяет, ждёт и освобождает задачу.

Поля `milib_task_result`: `state`, `kind`, `error`, `delay`, `ping`, `speed`,
`exit_ip`, `pings`, `ping_count`.

## Маршрутизация трафика

Через глобальный mixed inbound и выбранную ссылку:

```bash
milib_select_proxy(instance, "my-node");
curl -x http://127.0.0.1:<mixed_port> https://example.com
```

Через конкретную ссылку, независимо от выбора:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user milib:<password> https://example.com
```

Per-link SOCKS5 поддерживает TCP. UDP не проксируется.

## Как проверить ping из C

Готовый пример — `tests/c/test_ping.c`:

```bash
cc tests/c/test_ping.c -Iinclude -Ldist -lmilib \
   -Wl,-rpath,'$ORIGIN' -o dist/test_ping

./dist/test_ping 'vless://uuid@host:443?...#node' \
                 'https://www.gstatic.com/generate_204'
```

Или через переменные окружения:

```bash
MILIB_TEST_VLESS='vless://...' ./dist/test_ping
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
milib_link_destroy(link);
```

Удаляет прокси из `MILIB`, закрывает SOCKS-listener, отменяет и ждёт async
задачи этой ссылки, рвёт соединения с цепочкой прокси и освобождает handle.
После вызова handle использовать нельзя. Прокси, принадлежащий ссылке, нельзя
удалить через `milib_remove_proxy` — вернётся ошибка, а в лог будет подсказка
использовать `milib_link_destroy`.

## Потоки и безопасность

- операции с `milib_instance*` и `milib_link*` можно вызывать из разных потоков;
- `milib_close` и `milib_link_destroy` должны быть эксклюзивными: не
  вызывайте их одновременно с другими операциями над тем же объектом;
- callback приходит на произвольном потоке; для UI используйте poll или
  перекладывайте результат в свою очередь;
- log callback тоже вызывается на произвольном потоке;
- не вызывайте `milib_close`/destroy из callback;
- panic в фоновой горутине mihomo не перехватывается — это ограничение
  in-process библиотеки.
