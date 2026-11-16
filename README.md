<img src="assets/icon.svg" width="64" height="64" alt="">

# mewnet

Run the [mihomo](https://github.com/MetaCubeX/mihomo) proxy core **inside your
own process**. No daemon, no control port, no JSON: a C ABI with plain
functions, value structs and opaque handles that you can call from C, Python,
Odin or anything else with FFI.

The library bundles mihomo v1.19.33 plus everything needed around it:

- a local mixed SOCKS5/HTTP inbound on `127.0.0.1`;
- proxies added straight from share links (VLESS, VMess, Trojan, Shadowsocks,
  Hysteria2, ...);
- **SharedVPNLink** — one proxy exposed as its own authenticated SOCKS5 endpoint
  with `ping`, `speed` and `exit_ip` methods;
- latency, throughput and exit-IP checks that measure real traffic through the
  tunnel, not ICMP;
- async tasks with a callback or with polling;
- subscription storage in SQLite, a `+1/-1` rating with daily caps, and a
  discovery agent that keeps one working proxy selected and fails over
  automatically;
- four clients built on the same library: a CLI/TUI, `mewnet-watch`, a GTK4
  control panel, and an Odin/raylib GUI.

## Quick start

Build the core and the clients, give the CLI a subscription, and use the proxy:

```bash
make deps            # pinned toolchain via mise (go, clang, cmake, odin, android-sdk)
make subs            # dist/libmewnet.so + CLI / watch / subs
./build/apps/mewnet-cli/mewnet-cli --headless \
  --db data/mewnet-cli.db --proxy-port 18085 \
  https://example.com/subscription.txt
```

The CLI prints when it has a working node:

```text
DB data/mewnet-cli.db
PROXY http://127.0.0.1:18085
ACTIVE id=6 name=some-node delay=525
READY local=http://127.0.0.1:18085
```

From there any HTTP client can use it:

```bash
curl -x http://127.0.0.1:18085 https://example.com
curl -x http://127.0.0.1:18085 https://api.ipify.org    # exit IP
```

## Choose your entry point

| You want | Start here |
|---|---|
| A proxy on localhost from a subscription | `mewnet-cli` (above) or the GTK panel |
| Proxy control from C | [`mewnet.h`](libs/mewnet/cmd/mewnet/mewnet.h) and [`docs/USAGE.md`](docs/USAGE.md) |
| Proxy control from Python | [`docs/USAGE.md`](docs/USAGE.md#python) |
| Proxy control from Go | [`libs/mewnet/README.md`](libs/mewnet/README.md) |
| Subscription storage and rating | [`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md) |
| One proxy with its own SOCKS5 auth | [`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md) |
| How the agent picks and switches nodes | [`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md) |
| Build and cross-compile details | [`docs/BUILD.md`](docs/BUILD.md) |

## Build

Requirements: Linux x86_64 with glibc, Go 1.26+ with CGO, a C compiler.
Consumers of the finished library need none of that — only `libmewnet.so` and
the two headers.

```bash
make deps            # install go/clang/cmake/odin via mise
make build           # dist/libmewnet.so + dist/libmewnet.h
make subs            # the same library + C clients and tests
```

Cross-compile the core for Windows:

```bash
make build-mingw-core    # dist/windows/mewnet.dll + import library
make subs-mingw          # one DLL for all clients
```

Full toolchain notes, flags and caveats: [`docs/BUILD.md`](docs/BUILD.md).

## Use it from C

Errors are returned, not thrown: `mewnet_init` and `mewnet_link_create` hand
back a full `mewnet_error`, other operations return an error code that also
reaches the log callback.

```c
#include "mewnet.h"

mewnet_config config = mewnet_config_default();
config.mixed_port = 0;          /* 0: free port (or YAML), -1: no inbound */
config.log_level = "warning";

mewnet_init_result init = mewnet_init(&config);
if (init.instance == NULL) {
    fprintf(stderr, "init: %s\n", init.error.message);
    return 1;
}
mewnet_instance *m = init.instance;

mewnet_add_options add = mewnet_add_options_default();
add.select_proxy = true;
if (mewnet_add_proxy(m, "vless://...", &add) != MEWNET_ERROR_NONE) {
    /* details go to the log callback */
}

mewnet_delay_options delay = mewnet_delay_options_default();
delay.url = "https://www.gstatic.com/generate_204";
delay.expected = "204";
delay.timeout_ms = 5000;

mewnet_delay_result test = mewnet_test_delay(m, "some-node", &delay);
if (test.ok) {
    printf("delay: %u ms\n", test.delay_ms);
}

mewnet_close(m);
```

Everything else — listing proxies, list ownership, subscriptions, streaming
HTTP, logging, async patterns — is in [`docs/USAGE.md`](docs/USAGE.md) and the
header comments.

## SharedVPNLink

`mewnet_link_create` turns one share link into an object you can hand to any
SOCKS5 client:

1. the proxy is added to the `MEWNET` group and can be hot-swapped later with
   `mewnet_select_proxy`;
2. a SOCKS5 listener opens on `127.0.0.1:<random>` **with authentication**.

The login defaults to `mewnet`; the password is generated (18 random bytes)
unless you pass `password`. The upstream credential (VLESS UUID, Trojan
password) can be overridden with `proxy_password`.

```c
mewnet_link_options options = mewnet_link_options_default();
options.local_proxy = true;

mewnet_link_result created = mewnet_link_create(m, "vless://...", &options);
mewnet_link *link = created.link;

mewnet_link_info info = mewnet_link_get_info(link);
printf("socks5 %s user=%s pass=%s\n",
       info.socks_addr, info.socks_username, info.socks_password);

mewnet_ping_result ping = mewnet_link_ping(link, &delay);
mewnet_speed_result speed = mewnet_link_speed(link, &speed_options);
mewnet_exit_ip_result ip = mewnet_link_exit_ip(link, &exit_options);

mewnet_link_destroy(link);
```

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<generated-password> https://example.com
```

Per-link endpoints proxy TCP only. Details and edge cases:
[`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md).

## Async tasks

Every check has an async variant. Pass a callback, or pass `NULL` and poll.

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

Callbacks run on an arbitrary OS thread. Do not call `mewnet_close`,
`mewnet_link_destroy` or `mewnet_task_destroy` from inside one. For a
main-thread loop, use `mewnet_task_poll` instead:

```c
mewnet_task *task = mewnet_link_ping_async(link, &delay, NULL, NULL);
mewnet_task_result result;

while ((result = mewnet_task_poll(task)).state == MEWNET_TASK_RUNNING) {
    /* main loop */
}
mewnet_task_destroy(task);
```

`mewnet_link_destroy` and `mewnet_close` cancel and wait for their tasks.

## Subscriptions

Subscription storage, SQLite and the discovery agent live in the same
`libmewnet`; there is no separate `libmewnet_subs`. `mewnet-subs` is the
command-line front end:

```bash
make subs
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 0 name
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db run --limit 10 --window 240
```

It streams plain and base64 subscriptions, stores configs in SQLite, groups them
(there is always an `All` group), deduplicates within a group by the full link
without `#name`, tests configs under a shared rate limit (10 requests per
240 seconds by default) and keeps a `+1/-1` rating with daily caps.

Details: [`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md).

## Clients

**`mewnet-watch`** — discovery from the terminal. Checks up to 70 proxies at
once until one works, then prints the local proxy address:

```bash
./build/apps/mewnet-watch/mewnet-watch --db data/watch.db --proxy-port 18085 \
  --batch 70 --concurrency 70 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
```

**`mewnet-cli`** — the same agent with a TUI (`s` switches proxy, `q` quits) or
`--headless` for scripts:

```bash
./build/apps/mewnet-cli/mewnet-cli --db data/mewnet-cli.db --proxy-port 18085
```

**`mewnet-gtk`** — GTK4 control panel: add and remove subscriptions, change the
active node, watch the discovery log.

```bash
make gtk
./build/apps/mewnet-gtk/mewnet-gtk --db data/mewnet-gtk.db --proxy-port 18085
```

**`mewnet-gui`** — a small Odin/raylib GUI for browsing subscription groups and
paging through proxies (`make gui`).

The reliability policy (health checks every 6 s, failover after 2 failures out
of 3, pool of 12, random switch among the best) is described in
[`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md).

## Repository layout

```text
libs/mewnet/              Go core, built as the c-shared libmewnet
libs/mewnet/cmd/mewnet/   one Go package: Go core + all C subscription code + SQLite
apps/gui/                 Odin + raylib GUI
apps/mewnet-gtk/          C + GTK4 control panel
apps/mewnet-cli/          C CLI/TUI agent
apps/mewnet-watch/        C discovery tool
tests/                    C, Python and e2e tests
docs/                     usage, building, testing, design notes
```

## Limitations

- one mihomo instance per process (the core keeps global state);
- `dlclose` on the `.so` is not allowed: the Go runtime cannot be unloaded;
- no TUN/system VPN in this build (needs the `with_gvisor` tag);
- "ping" is HTTP latency via `URLTest`, not ICMP;
- per-link SOCKS5 proxies TCP only;
- `mewnet_close` and `mewnet_link_destroy` must not be called from an async
  callback;
- a panic in a background mihomo goroutine kills the process — run untrusted
  configs in a separate process;
- the build depends on glibc; for old distributions build in an older image.

## Tests

```bash
make test-unit       # ABI and logic through ctypes
make test-links      # SharedVPNLink without network (auth, errors, destroy)
make test-c          # C test with #include "mewnet.h"
make test-stress     # 100 add/remove cycles, FD/goroutine/RSS
make test-agent      # offline failover/recovery test for the agent
make xray            # download Xray for e2e (into /tmp, not committed)
make test-e2e        # local Xray + VLESS + mixed inbound
make test-e2e-links  # 4 proxies, hot-swap, concurrent pings
make test-e2e-subs   # local Xray + HTTP subscription + SQLite/rating
make test-c-ping-local
```

Results, numbers and known gaps: [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md).

## License

GPL-3.0, same as mihomo. Distributing the `.so` means complying with the GPL.
