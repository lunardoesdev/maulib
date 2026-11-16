<img src="assets/icon.svg" width="64" height="64" alt="">

# mewnet

Run the [mihomo](https://github.com/MetaCubeX/mihomo) proxy core **inside your
own process**. No daemon, no control port, no JSON: a C ABI with plain
functions, value structs and opaque handles that you can call from C, Python,
Odin or anything else with FFI.

You get a local proxy on `127.0.0.1`, per-proxy measurements, subscription
storage in SQLite and a discovery agent that keeps a working node selected — all
inside your binary.

## How it fits together

There is **one core instance per process**. Inside that core:

- a **mixed SOCKS5/HTTP inbound** listens on `127.0.0.1:<port>`;
- a selector group called **`MEWNET`** holds every proxy you add;
- the rules end with `MATCH,MEWNET`, so all traffic that enters the inbound goes
  to whichever proxy is currently selected.

```text
your client ──► 127.0.0.1:18085 (mixed inbound)
                        │
                 [ MEWNET selector ]
                        │
        ┌───────────────┼───────────────┐
   proxy A (selected)  proxy B        proxy C
```

On top of that there are **two independent ways to route traffic**. This is the
part that usually confuses people, so here is the whole model in one table:

| | Shared inbound | Per-link endpoint (SharedVPNLink) |
|---|---|---|
| What listens | one mixed port for the whole process | one SOCKS5 port per link |
| Which proxy is used | the currently **selected** proxy in `MEWNET` | **exactly the proxy of that link**, always |
| Credentials | none | user `mewnet` + generated password (overridable) |
| How you change the exit | `mewnet_select_proxy` / `add.select_proxy` | create another link, or destroy this one |
| Lifetime | until `mewnet_close` | until `mewnet_link_destroy` |
| Created by | `mewnet_init` | `mewnet_link_create` |
| Typical use | one proxy for the whole app | give one thread/process/subscriber its own exit |

Two things that follow from this:

1. **A link is not a second tunnel.** `mewnet_link_create` adds its proxy to the
   same `MEWNET` group, so the proxy can be used both by the shared inbound (if
   you select it) and by its own SOCKS5 port.
2. **Per-link traffic ignores the selection.** If the app switches to proxy C,
   a client connected to link B's SOCKS5 port keeps exiting through B.

## Quick start

Build the core and the clients, give the CLI a subscription, and use the proxy:

```bash
make deps            # pinned toolchain via mise (go, clang, cmake, odin, android-sdk)
make subs            # dist/libmewnet.so + CLI / watch / subs
./build/apps/mewnet-cli/mewnet-cli --headless \
  --db data/mewnet-cli.db --proxy-port 18085 \
  https://example.com/subscription.txt
```

The CLI is the core plus subscriptions plus the agent, so it prints the mixed
port as soon as a node works:

```text
DB data/mewnet-cli.db
PROXY http://127.0.0.1:18085
ACTIVE id=6 name=some-node delay=525
READY local=http://127.0.0.1:18085
```

```bash
curl -x http://127.0.0.1:18085 https://example.com
curl -x http://127.0.0.1:18085 https://api.ipify.org    # exit IP
```

## Route traffic through a proxy

### Path 1 — shared inbound: traffic goes through the selected proxy

```c
mewnet_add_options add = mewnet_add_options_default();
add.select_proxy = true;                 /* make it the selected proxy */
mewnet_add_proxy(m, "vless://...", &add);

int port = mewnet_get_status(m).mixed_port;
printf("use http://127.0.0.1:%d\n", port);
```

Now everything pointed at `http://127.0.0.1:<port>` (or `socks5h://`, the same
port speaks SOCKS5) exits through that proxy:

```bash
curl -x http://127.0.0.1:$PORT https://example.com
```

Switching later is a hot swap: `mewnet_select_proxy(m, "other-node")`. New
connections use the new node; connections that are already open stay on the old
one until they close.

### Path 2 — per-link endpoint: traffic goes through one specific link

Use this when a consumer needs its own exit and its own credentials, without
caring what the rest of the process has selected.

```c
mewnet_link_options opts = mewnet_link_options_default();
opts.local_proxy = true;                 /* default: open a SOCKS5 port */

mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
if (created.link == NULL) { /* created.error.message */ }

mewnet_link_info info = mewnet_link_get_info(created.link);
printf("%s %s %s\n", info.socks_addr, info.socks_username, info.socks_password);
```

That is all — traffic sent to `info.socks_addr` with those credentials always
exits through this link:

```bash
curl --socks5-hostname 127.0.0.1:<link_port> \
     --proxy-user mewnet:<generated-password> https://example.com
```

`mewnet_link_ping`, `mewnet_link_speed` and `mewnet_link_exit_ip` also measure
this link specifically, regardless of which proxy is selected.

### Which one do I need?

| Case | Use |
|---|---|
| Whole application goes through one proxy | shared inbound + selection |
| Different subscribers/threads must exit through different proxies | one link per subscriber |
| You want to hand a ready-made SOCKS5 endpoint to another program | per-link endpoint |
| You only need to test links and pick the fastest | links + `mewnet_link_ping_many`, no routing needed |
| Subscription-driven, failover must happen automatically | the agent with the shared inbound |

## C: complete example, shared inbound

```c
#include <stdio.h>
#include "mewnet.h"

int main(void) {
    mewnet_config config = mewnet_config_default();
    config.mixed_port = 0;           /* 0: pick a free port, -1: no inbound */
    config.log_level = "warning";

    mewnet_init_result init = mewnet_init(&config);
    if (init.instance == NULL) {
        fprintf(stderr, "init failed (%d): %s\n", init.error.code, init.error.message);
        return 1;
    }
    mewnet_instance *m = init.instance;

    mewnet_add_options add = mewnet_add_options_default();
    add.select_proxy = true;
    if (mewnet_add_proxy(m, "vless://...", &add) != MEWNET_ERROR_NONE) {
        fprintf(stderr, "add failed, see the log callback\n");
        mewnet_close(m);
        return 1;
    }

    mewnet_delay_options delay = mewnet_delay_options_default();
    delay.url = "https://www.gstatic.com/generate_204";
    delay.expected = "204";
    delay.timeout_ms = 5000;

    mewnet_status_info status = mewnet_get_status(m);
    printf("proxy: http://127.0.0.1:%d (selected: %s)\n",
           status.mixed_port, status.selected);

    mewnet_delay_result d = mewnet_test_delay(m, status.selected, &delay);
    if (d.ok) {
        printf("delay: %u ms\n", d.delay_ms);
    }

    /* ... the app runs, traffic goes through http://127.0.0.1:<mixed_port> ... */

    mewnet_close(m);
    return 0;
}
```

Compile:

```bash
cc app.c -Ilibs/mewnet/cmd/mewnet -Ldist -lmewnet -Wl,-rpath,"$PWD/dist" -o app
./app
```

Buffer sizes, list ownership and the rest of the API are in
[`docs/USAGE.md`](docs/USAGE.md) and the header comments.

## C: complete example, one link with its own SOCKS5

```c
#include <stdio.h>
#include "mewnet.h"

int main(void) {
    mewnet_config config = mewnet_config_default();
    config.mixed_port = -1;          /* no shared inbound at all */

    mewnet_init_result init = mewnet_init(&config);
    if (init.instance == NULL) {
        fprintf(stderr, "init: %s\n", init.error.message);
        return 1;
    }
    mewnet_instance *m = init.instance;

    mewnet_link_options opts = mewnet_link_options_default();
    opts.local_proxy = true;

    mewnet_link_result created = mewnet_link_create(m, "vless://...", &opts);
    if (created.link == NULL) {
        fprintf(stderr, "link: %s\n", created.error.message);
        mewnet_close(m);
        return 1;
    }

    mewnet_link_info info = mewnet_link_get_info(created.link);
    printf("curl --socks5-hostname %s --proxy-user %s:%s https://example.com\n",
           info.socks_addr, info.socks_username, info.socks_password);

    mewnet_delay_options delay = mewnet_delay_options_default();
    mewnet_ping_result ping = mewnet_link_ping(created.link, &delay);
    printf("delay: %u ms\n", ping.delay_ms);

    mewnet_link_destroy(created.link);
    mewnet_close(m);
    return 0;
}
```

`local_proxy = false` skips the SOCKS5 listener when you only need the
measurements. Edge cases, auth rules and hot-swap details:
[`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md).

## Subscriptions and the agent

A subscription is a URL that returns share links. `libmewnet` downloads it,
stores configs in SQLite, tests them under a shared rate limit, rates them
`+1/-1`, and the agent keeps a pool of working nodes and switches the selected
proxy when the active one dies.

```bash
make subs
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db sub add URL 0 name
./build/apps/mewnet-subs/mewnet-subs --db data/subs.db run --limit 10 --window 240
```

The shortest path from a subscription to working traffic is the CLI from Quick
start: it opens the database, starts the core, starts the agent and prints the
mixed port. In your own program the same pieces are `mewnet_subs_*` plus
`mewnet_agent_start`; see [`docs/USAGE.md`](docs/USAGE.md) and
[`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md).

## Async

Every check has an async variant: pass a callback, or `NULL` and poll.

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

Callbacks run on an arbitrary OS thread; do not call `mewnet_close`,
`mewnet_link_destroy` or `mewnet_task_destroy` from inside one. For a
main-thread loop, create the task with `NULL` and use `mewnet_task_poll`.

## Clients

**`mewnet-cli`** — the agent with a TUI (`s` switches proxy, `q` quits) or
`--headless` for scripts.

**`mewnet-watch`** — discovery only: checks up to 70 proxies at once until one
works, prints `PROXY http://127.0.0.1:<port>`.

**`mewnet-gtk`** — GTK4 panel: add and remove subscriptions, change the active
node, watch the discovery log.

**`mewnet-gui`** — a small Odin/raylib GUI for browsing groups and paging
through proxies (`make gui`).

All four use the same library and the same agent, so the behaviour matches.
Policy (health checks every 6 s, failover after 2 failures out of 3, pool of 12,
random switch among the best): [`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md).

## Build

Requirements: Linux x86_64 with glibc, Go 1.26+ with CGO, a C compiler.
Consumers of the finished library need none of that — only `libmewnet.so` and
the headers.

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

## Choose your entry point

| You want | Start here |
|---|---|
| A proxy on localhost from a subscription | `mewnet-cli` (Quick start) or the GTK panel |
| Proxy control from C | [`mewnet.h`](libs/mewnet/cmd/mewnet/mewnet.h), [`docs/USAGE.md`](docs/USAGE.md) |
| Proxy control from Python | [`docs/USAGE.md`](docs/USAGE.md#python) |
| Proxy control from Go | [`libs/mewnet/README.md`](libs/mewnet/README.md) |
| Subscription storage and rating | [`docs/SUBSCRIPTIONS.md`](docs/SUBSCRIPTIONS.md) |
| One proxy with its own SOCKS5 auth | [`docs/SHARED_VPN_LINK.md`](docs/SHARED_VPN_LINK.md) |
| How the agent picks and switches nodes | [`docs/GTK_RELIABILITY.md`](docs/GTK_RELIABILITY.md) |
| Build and cross-compile details | [`docs/BUILD.md`](docs/BUILD.md) |

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
- switching the selected proxy does not migrate connections that are already
  open — they finish on the old node;
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
