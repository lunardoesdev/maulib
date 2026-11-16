# SharedVPNLink API (v2)

`SharedVPNLink` is an object created from a share link (`vless://`, `vmess://`,
`trojan://`, `ss://`, ...). It combines:

1. a proxy in the `MEWNET` group (participates in hot-swap via `mewnet_select_proxy`);
2. a local SOCKS5 endpoint on `127.0.0.1:<random>` with mandatory
   authentication;
3. the `ping`, `speed`, `exit_ip` methods and their async variants.

Headers: `libs/mewnet/cmd/mewnet/mewnet.h`, `libs/mewnet/cmd/mewnet/mewnet_types.h`.
ABI v2 — C structs, no JSON strings. Errors are returned in the result, and for
simple operations as a `mewnet_error_code` code.

## Creation

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

`mewnet_link_options_default()` sets `local_proxy = true` and
`select_proxy = false`. Do not use a fully zeroed struct if you need a local
SOCKS5: take the default and change the fields.

| Field | Default | Meaning |
|---|---|---|
| `name` | fragment / type-server-port | name of the proxy and the link |
| `select_proxy` | `false` | select the proxy in `MEWNET` immediately |
| `local_proxy` | `true` | bring up a per-link SOCKS5 |
| `username` | `mewnet` | login for the local SOCKS5 |
| `password` | 18 random bytes | password for the local SOCKS5 |
| `proxy_password` | from the link | override the upstream credential |

`proxy_password` substitutes the credential depending on the protocol:

- VLESS/VMess → `uuid`;
- Trojan/Shadowsocks/Hysteria/... → `password`;
- SOCKS5/HTTP → `username`/`password`.

## Info

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

Fields: `ok`, `error`, `id`, `name`, `proxy_type`, `server`, `port`, `uri`,
`socks_addr`, `socks_username`, `socks_password`, `selected`.

List of all links:

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

The delay is measured with **traffic through the proxy** (`URLTest`: HTTP HEAD
over VLESS/...), not with an ICMP ping to the proxy server.

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

Result fields: `ok`, `error`, `status`, `bytes`, `seconds`,
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

Options: `url` (default `https://api.ipify.org`), `format`
(`MEWNET_EXIT_IP_TEXT` / `MEWNET_EXIT_IP_JSON`), `timeout_ms`.

## Many proxies at once

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

All pings run in separate goroutines; the result preserves the order of the
input link array.

## Async

The callback receives a `mewnet_task_result`:

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

Or poll without a callback:

```c
mewnet_task *task = mewnet_link_ping_async(link, &delay, NULL, NULL);
mewnet_task_result result;

while ((result = mewnet_task_poll(task)).state == MEWNET_TASK_RUNNING) {
    /* main loop */
}
mewnet_task_destroy(task);
```

Rules:

- the callback is invoked on an arbitrary OS thread;
- do not call `mewnet_close`, `mewnet_link_destroy`, `mewnet_task_destroy`
  from the callback;
- `mewnet_link_destroy` cancels and waits for this link's tasks;
- `mewnet_close` cancels and waits for all tasks of the instance;
- `mewnet_task_cancel` only requests cancellation;
- `mewnet_task_destroy` cancels, waits for and frees the task.

Fields of `mewnet_task_result`: `state`, `kind`, `error`, `delay`, `ping`,
`speed`, `exit_ip`, `pings`, `ping_count`.

## Traffic routing

Through the global mixed inbound and the selected link:

```bash
mewnet_select_proxy(instance, "my-node");
curl -x http://127.0.0.1:<mixed_port> https://example.com
```

Through a specific link, regardless of the selection:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<password> https://example.com
```

Per-link SOCKS5 supports TCP. UDP is not proxied.

## How to test ping from C

A ready-made example is `tests/c/test_ping.c`:

```bash
cc tests/c/test_ping.c -Iinclude -Ldist -lmewnet \
   -Wl,-rpath,'$ORIGIN' -o dist/test_ping

./dist/test_ping 'vless://uuid@host:443?...#node' \
                 'https://www.gstatic.com/generate_204'
```

Or via environment variables:

```bash
MEWNET_TEST_VLESS='vless://...' ./dist/test_ping
```

A check without a real link (brings up a local Xray and a `204` target):

```bash
make test-c-ping-local
```

Example output:

```text
3
```

That is the delay in milliseconds.

## Removal

```c
mewnet_link_destroy(link);
```

Removes the proxy from `MEWNET`, closes the SOCKS listener, cancels and waits
for this link's async tasks, tears down connections to the proxy chain and frees
the handle. After the call the handle must not be used. A proxy owned by a link
cannot be removed via `mewnet_remove_proxy` — an error is returned, and the log
suggests using `mewnet_link_destroy`.

## Threads and safety

- operations on `mewnet_instance*` and `mewnet_link*` can be called from
  different threads;
- `mewnet_close` and `mewnet_link_destroy` must be exclusive: do not call them
  concurrently with other operations on the same object;
- the callback arrives on an arbitrary thread; for UI use poll or move the
  result into your own queue;
- the log callback is also invoked on an arbitrary thread;
- do not call `mewnet_close`/destroy from the callback;
- a panic in a mihomo background goroutine is not caught — that is a limitation
  of the in-process library.
